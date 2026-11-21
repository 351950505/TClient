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
		int m_PendingTileIndex = -1;
		int m_PendingFreezeEnd = -1;

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
		float Update(bool Active, bool Frozen, int FreezeEnd, int CurrentTick, float Now, bool Enabled, FGetFreezeTile GetFreezeTile)
		{
			if(!Active)
			{
				m_StateKnown = false;
				m_WasFrozen = false;
				m_FlashStart = -1.0f;
				m_TileIndex = -1;
				m_PendingTileIndex = -1;
				m_PendingFreezeEnd = -1;
				return 0.0f;
			}

			if(!m_StateKnown)
			{
				m_StateKnown = true;
				m_WasFrozen = Frozen;
				return 0.0f;
			}

			if(!Enabled)
			{
				m_WasFrozen = Frozen;
				m_FlashStart = -1.0f;
				m_TileIndex = -1;
				m_PendingTileIndex = -1;
				m_PendingFreezeEnd = -1;
				return 0.0f;
			}

			if(!m_WasFrozen && Frozen)
			{
				m_PendingTileIndex = -1;
				m_PendingFreezeEnd = -1;
				if(FreezeEnd > 0)
				{
					m_PendingTileIndex = GetFreezeTile();
					if(m_PendingTileIndex >= 0)
						m_PendingFreezeEnd = FreezeEnd;
				}
			}
			else if(m_WasFrozen && Frozen && m_PendingTileIndex >= 0)
			{
				if(FreezeEnd <= 0)
				{
					m_PendingTileIndex = -1;
					m_PendingFreezeEnd = -1;
				}
				else if(FreezeEnd > m_PendingFreezeEnd)
				{
					m_PendingFreezeEnd = FreezeEnd;
				}
			}
			else if(m_WasFrozen && !Frozen)
			{
				const bool FreezeCompleted = m_PendingTileIndex >= 0 && m_PendingFreezeEnd > 0 && CurrentTick >= m_PendingFreezeEnd;
				if(FreezeCompleted && Now >= m_CooldownUntil)
				{
					m_FlashStart = Now;
					m_CooldownUntil = Now + COOLDOWN_SECONDS;
					m_TileIndex = m_PendingTileIndex;
				}
				m_PendingTileIndex = -1;
				m_PendingFreezeEnd = -1;
			}

			m_WasFrozen = Frozen;
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
