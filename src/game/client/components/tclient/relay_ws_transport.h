#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_RELAY_WS_TRANSPORT_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_RELAY_WS_TRANSPORT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace RelayWs
{
	struct CEvent
	{
		enum class EType
		{
			CONNECTED,
			DISCONNECTED,
			SIGNAL,
			ACK
		};
		EType m_Type = EType::DISCONNECTED;
		std::string m_Payload;
		std::string m_Uid;
		std::string m_Session;
		uint64_t m_Sequence = 0;
		int m_Recipients = 0;
	};

	struct CStatus
	{
		bool m_Online = false;
		size_t m_Queued = 0;
		std::string m_Error;
	};

	// Start/Stop only replace local state; they never perform network I/O or join.
	// Stop clears all events/sends, including publications from the old generation.
	// Destruction cancels and joins the owned worker; destroy outside the render tick.
	class CTransport
	{
		class CImpl;
		std::unique_ptr<CImpl> m_pImpl;

	public:
		CTransport();
		~CTransport();
		CTransport(const CTransport &) = delete;
		CTransport &operator=(const CTransport &) = delete;

		// ASCII DNS/IPv4 host, optional :443, and either no path or exactly /ws.
		// Room: 1..64 ASCII alphanumeric, '.', '[', ']', ':'. Uid: 16 lower hex.
		void Start(const std::string &Url, const std::string &Room, const std::string &Uid, const std::string &Key);
		void Stop();
		// Copies exact, nonempty UTF-8 (at most 160 bytes), only while welcomed.
		// true means queued, not delivered. ACK counts accepted server socket writes.
		bool Send(const char *pPayload);
		std::vector<CEvent> Drain();
		CStatus Status() const;
	};
} // namespace RelayWs

#endif
