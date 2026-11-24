#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_FROZEN_ENTRY_HIGHLIGHT_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_FROZEN_ENTRY_HIGHLIGHT_H

namespace tclient_frozen_entry
{
	inline constexpr float FLASH_DURATION_SECONDS = 0.5f;

	inline bool IsPrimaryLocalClient(int ClientId, int PrimaryLocalClientId)
	{
		return PrimaryLocalClientId >= 0 && ClientId == PrimaryLocalClientId;
	}

	class CHighlight
	{
		bool m_StateKnown = false;
		bool m_WasFrozen = false;
		bool m_WasEnabled = false;
		bool m_Suspended = false;
		int m_TileIndex = -1;
		float m_ExpireTime = 0.0f;
		float m_TimeLeft = 0.0f;

	public:
		void Reset()
		{
			*this = CHighlight();
		}

		int TileIndex() const { return m_Suspended ? -1 : m_TileIndex; }
		float Alpha() const { return !m_Suspended && m_TileIndex >= 0 ? m_TimeLeft / FLASH_DURATION_SECONDS : 0.0f; }
		void Suspend() { m_Suspended = true; }

		template<typename FGetFreezeTile>
		float Update(bool Active, bool Frozen, bool Enabled, FGetFreezeTile GetFreezeTile, float Now = 0.0f)
		{
			if(!Active)
			{
				Reset();
				return 0.0f;
			}

			m_Suspended = false;
			if(!Enabled)
			{
				m_StateKnown = true;
				m_WasFrozen = Frozen;
				m_WasEnabled = false;
				m_TileIndex = -1;
				m_ExpireTime = 0.0f;
				m_TimeLeft = 0.0f;
				return 0.0f;
			}

			if(!m_StateKnown || (Frozen && (!m_WasFrozen || !m_WasEnabled)))
			{
				m_TileIndex = Frozen ? GetFreezeTile() : -1;
				m_TimeLeft = m_TileIndex >= 0 ? FLASH_DURATION_SECONDS : 0.0f;
				m_ExpireTime = Now + m_TimeLeft;
			}
			else if(!Frozen)
			{
				m_TileIndex = -1;
				m_ExpireTime = 0.0f;
				m_TimeLeft = 0.0f;
			}
			else if(m_TileIndex >= 0)
			{
				m_TimeLeft = m_ExpireTime - Now;
				if(m_TimeLeft <= 0.0f)
				{
					m_TileIndex = -1;
					m_ExpireTime = 0.0f;
					m_TimeLeft = 0.0f;
				}
				else if(m_TimeLeft > FLASH_DURATION_SECONDS)
				{
					m_TimeLeft = FLASH_DURATION_SECONDS;
				}
			}

			m_StateKnown = true;
			m_WasFrozen = Frozen;
			m_WasEnabled = true;
			return Alpha();
		}
	};
}

#endif
