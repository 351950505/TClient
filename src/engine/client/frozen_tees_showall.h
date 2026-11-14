#ifndef ENGINE_CLIENT_FROZEN_TEES_SHOWALL_H
#define ENGINE_CLIENT_FROZEN_TEES_SHOWALL_H

#include <generated/protocol.h>

#include <algorithm>
#include <cstdint>

// Match CGameClient's m_EntitiesDDRace, but use the validated snapshot of the
// actual connection, never the active client's game info for the dummy.
inline bool TcFrozenTeesEntitiesDDRace(int InfoExSize, int Version, int Flags, bool FallbackDDRace)
{
	if(InfoExSize < 4)
		Version = -1;
	else if(InfoExSize < 8)
		Version = 0;
	else if(InfoExSize < 12)
		Version = std::min(Version, 4);

	if(Version < 1)
		return FallbackDDRace;
	if(Version >= 2)
		return (Flags & GAMEINFOFLAG_ENTITIES_DDRACE) != 0;
	return (Flags & (GAMEINFOFLAG_GAMETYPE_DDRACE | GAMEINFOFLAG_GAMETYPE_DDNET)) != 0;
}

// One native /showall 1 attempt per EnterGame and per connection. This records
// attempts, NOT server acknowledgement. Off never sends /showall 0 and does not
// re-arm a spent attempt. Poll only on verified snapshots, not on rendered frames.
class CFrozenTeesShowAll
{
	bool m_PostConnected = false;
	bool m_Attempted = false;
	bool m_Pending = false;
	int64_t m_NotBefore = 0;
	int64_t m_Deadline = 0;

public:
	static constexpr int CHAT_QUIET_SECONDS = 2;
	static constexpr int WAIT_SECONDS = 15;

	void Reset() { *this = CFrozenTeesShowAll(); }

	void OnPostConnect(int64_t Now, int64_t Frequency)
	{
		if(m_PostConnected || Frequency <= 0)
			return;
		m_PostConnected = true;
		// In particular, never put showall after timeout in its /mc string:
		// successful timeout recovery invalidates that command's original ID.
		m_NotBefore = std::max(m_NotBefore, Now + CHAT_QUIET_SECONDS * Frequency);
	}

	void OnChatSent(int64_t Now, int64_t Frequency)
	{
		if(Frequency > 0)
			m_NotBefore = std::max(m_NotBefore, Now + CHAT_QUIET_SECONDS * Frequency);
	}

	bool Update(int64_t Now, int64_t Frequency, bool Online, bool Requested, bool EntitiesDDRace)
	{
		if(!m_PostConnected || m_Attempted || !Online || Frequency <= 0)
			return false;
		// Start a bounded window on the first applicable request, including an
		// option enabled after joining. Repeated off/on keeps the same deadline.
		if(!m_Pending && Requested && EntitiesDDRace)
		{
			m_Pending = true;
			m_Deadline = Now + WAIT_SECONDS * Frequency;
		}
		if(!m_Pending)
			return false;
		if(Now >= m_Deadline)
		{
			m_Attempted = true; // Expired; don't keep retrying a busy/unsupported server.
			m_Pending = false;
			return false;
		}
		if(!Requested || !EntitiesDDRace || Now < m_NotBefore)
			return false;

		m_Attempted = true; // Consume BEFORE the caller sends (no acknowledgement).
		m_Pending = false;
		return true;
	}
};

#endif // ENGINE_CLIENT_FROZEN_TEES_SHOWALL_H
