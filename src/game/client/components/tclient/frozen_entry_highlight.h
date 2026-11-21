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

		float Alpha(float Now) const
		{
			if(m_FlashStart < 0.0f)
				return 0.0f;
			const float Elapsed = std::max(Now - m_FlashStart, 0.0f);
			return Elapsed >= FLASH_SECONDS ? 0.0f : 1.0f - Elapsed / FLASH_SECONDS;
		}

		template<typename FGetFreezeTile>
		float Update(bool Active, bool Frozen, float Now, bool Enabled, FGetFreezeTile GetFreezeTile)
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
			if(EnteredFreeze && Enabled && Now >= m_CooldownUntil)
			{
				const int FreezeTileIndex = GetFreezeTile();
				if(FreezeTileIndex >= 0)
				{
					m_FlashStart = Now;
					m_CooldownUntil = Now + COOLDOWN_SECONDS;
					m_TileIndex = FreezeTileIndex;
				}
			}

			if(m_FlashStart >= 0.0f && Now - m_FlashStart >= FLASH_SECONDS)
			{
				m_FlashStart = -1.0f;
				m_TileIndex = -1;
			}
			return Alpha(Now);
		}
	};
}

#endif
