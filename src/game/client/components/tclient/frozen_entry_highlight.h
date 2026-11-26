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
		bool m_EpisodeKnown = false;
		bool m_EpisodeFrozen = false;
		bool m_Armed = false;
		bool m_Suspended = false;
		int m_TileIndex = -1;
		float m_ExpireTime = 0.0f;
		float m_TimeLeft = 0.0f;

		void ClearHighlight()
		{
			m_TileIndex = -1;
			m_ExpireTime = 0.0f;
			m_TimeLeft = 0.0f;
		}

	public:
		void Reset()
		{
			*this = CHighlight();
		}

		int TileIndex() const { return m_Suspended ? -1 : m_TileIndex; }
		float Alpha() const { return !m_Suspended && m_TileIndex >= 0 ? m_TimeLeft / FLASH_DURATION_SECONDS : 0.0f; }

		template<typename FGetFreezeTile>
		float Update(bool Active, bool HasServerSnapshot, bool Frozen, bool Enabled, bool Visible, FGetFreezeTile GetFreezeTile, float Now = 0.0f)
		{
			m_Suspended = !Active || !HasServerSnapshot || !Visible;
			if(!Active || !HasServerSnapshot)
			{
				ClearHighlight();
				return 0.0f;
			}

			if(!Enabled)
				ClearHighlight();

			if(!m_EpisodeKnown)
			{
				m_EpisodeKnown = true;
				m_EpisodeFrozen = Frozen;
				m_Armed = !Frozen;
			}
			else if(Frozen && !m_EpisodeFrozen)
			{
				const bool HighlightEntry = m_Armed && Enabled && Visible;
				m_EpisodeFrozen = true;
				m_Armed = false;
				ClearHighlight();
				if(HighlightEntry)
				{
					m_TileIndex = GetFreezeTile();
					m_TimeLeft = m_TileIndex >= 0 ? FLASH_DURATION_SECONDS : 0.0f;
					m_ExpireTime = Now + m_TimeLeft;
				}
			}
			else if(!Frozen && m_EpisodeFrozen)
			{
				m_EpisodeFrozen = false;
				m_Armed = true;
				ClearHighlight();
			}

			if(m_EpisodeFrozen && m_TileIndex >= 0)
			{
				m_TimeLeft = m_ExpireTime - Now;
				if(m_TimeLeft <= 0.0f)
					ClearHighlight();
				else if(m_TimeLeft > FLASH_DURATION_SECONDS)
					m_TimeLeft = FLASH_DURATION_SECONDS;
			}

			return Alpha();
		}
	};
}

#endif
