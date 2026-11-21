#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_FROZEN_ENTRY_HIGHLIGHT_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_FROZEN_ENTRY_HIGHLIGHT_H

#include <algorithm>

namespace tclient_frozen_entry
{
	class CHighlight
	{
		bool m_StateKnown = false;
		bool m_WasFrozen = false;
		float m_CooldownUntil = 0.0f;
		float m_FlashStart = -1.0f;
		int m_TileIndex = -1;

	public:
		static constexpr float FLASH_SECONDS = 0.5f;
		static constexpr float COOLDOWN_SECONDS = 5.0f;

		void Reset()
		{
			*this = CHighlight();
		}

		int TileIndex() const { return m_TileIndex; }

		float Update(bool Active, bool Frozen, int FreezeTileIndex, float Now, bool Enabled)
		{
			if(!Active)
			{
				m_StateKnown = false;
				m_WasFrozen = false;
				m_FlashStart = -1.0f;
				m_TileIndex = -1;
				return 0.0f;
			}

			if(!m_StateKnown)
			{
				m_StateKnown = true;
				m_WasFrozen = Frozen;
				return 0.0f;
			}

			const bool EnteredFreeze = !m_WasFrozen && Frozen;
			m_WasFrozen = Frozen;
			if(!Enabled)
			{
				m_FlashStart = -1.0f;
				m_TileIndex = -1;
				return 0.0f;
			}
			if(EnteredFreeze && FreezeTileIndex >= 0 && Now >= m_CooldownUntil)
			{
				m_FlashStart = Now;
				m_CooldownUntil = Now + COOLDOWN_SECONDS;
				m_TileIndex = FreezeTileIndex;
			}

			if(m_FlashStart < 0.0f)
				return 0.0f;

			const float Elapsed = std::max(Now - m_FlashStart, 0.0f);
			if(Elapsed >= FLASH_SECONDS)
			{
				m_FlashStart = -1.0f;
				m_TileIndex = -1;
				return 0.0f;
			}
			return 1.0f - Elapsed / FLASH_SECONDS;
		}
	};
}

#endif
