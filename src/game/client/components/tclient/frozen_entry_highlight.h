#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_FROZEN_ENTRY_HIGHLIGHT_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_FROZEN_ENTRY_HIGHLIGHT_H

namespace tclient_frozen_entry
{
	class CHighlight
	{
		bool m_StateKnown = false;
		bool m_WasFrozen = false;
		bool m_WasEnabled = false;
		int m_TileIndex = -1;

	public:
		void Reset()
		{
			*this = CHighlight();
		}

		int TileIndex() const { return m_TileIndex; }
		float Alpha() const { return m_TileIndex >= 0 ? 1.0f : 0.0f; }

		template<typename FGetFreezeTile>
		float Update(bool Active, bool Frozen, bool Enabled, FGetFreezeTile GetFreezeTile)
		{
			if(!Active)
			{
				Reset();
				return 0.0f;
			}

			if(!Enabled)
			{
				m_StateKnown = true;
				m_WasFrozen = Frozen;
				m_WasEnabled = false;
				m_TileIndex = -1;
				return 0.0f;
			}

			if(!m_StateKnown || (Frozen && (!m_WasFrozen || !m_WasEnabled)))
				m_TileIndex = Frozen ? GetFreezeTile() : -1;
			else if(!Frozen)
				m_TileIndex = -1;

			m_StateKnown = true;
			m_WasFrozen = Frozen;
			m_WasEnabled = true;
			return Alpha();
		}
	};
}

#endif
