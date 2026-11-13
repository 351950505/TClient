#include "relay_ws_transport.h"

#include <base/detect.h>

#if defined(CONF_FAMILY_WINDOWS)
#include <base/secure.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

namespace RelayWs
{
	namespace
	{
		using CClock = std::chrono::steady_clock;
		using namespace std::chrono_literals;
		constexpr size_t MAX_PAYLOAD = 160;
		constexpr size_t MAX_FRAME = 512;
		constexpr size_t MAX_SENDS = 8;
		constexpr size_t MAX_EVENTS = 128;

		bool IsHexId(std::string_view Text)
		{
			return Text.size() == 16 && std::all_of(Text.begin(), Text.end(), [](char c) {
				return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
			});
		}

		bool IsUtf8(std::string_view Text)
		{
			return Text.find('\0') == Text.npos && !Text.empty() &&
			       MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(), static_cast<int>(Text.size()), nullptr, 0) != 0;
		}

		bool Number(std::string_view Text, uint64_t &Value)
		{
			if(Text.empty())
				return false;
			const auto Result = std::from_chars(Text.data(), Text.data() + Text.size(), Value);
			return Result.ec == std::errc() && Result.ptr == Text.data() + Text.size();
		}

		std::wstring Wide(std::string_view Text)
		{
			// Configuration has already been restricted to ASCII.
			return std::wstring(Text.begin(), Text.end());
		}

		std::string RandomId()
		{
			std::array<unsigned char, 8> aBytes{};
			secure_random_fill(aBytes.data(), static_cast<unsigned>(aBytes.size()));
			std::string Result(16, '0');
			constexpr char aHex[] = "0123456789abcdef";
			for(size_t i = 0; i < aBytes.size(); ++i)
			{
				Result[2 * i] = aHex[aBytes[i] >> 4];
				Result[2 * i + 1] = aHex[aBytes[i] & 15];
			}
			return Result;
		}

		struct CConfig
		{
			std::string m_Host;
			std::string m_Room;
			std::string m_Uid;
			std::string m_Key;
			~CConfig()
			{
				if(!m_Key.empty())
					SecureZeroMemory(m_Key.data(), m_Key.size());
			}
		};

		bool Configure(const std::string &Url, const std::string &Room, const std::string &Uid, const std::string &Key, CConfig &Config)
		{
			if(Url.size() > 280 || Url.compare(0, 6, "wss://") != 0 || Room.empty() || Room.size() > 64 || !IsHexId(Uid) || Key.size() > 4096)
				return false;
			if(!std::all_of(Room.begin(), Room.end(), [](unsigned char c) {
				   return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '[' || c == ']' || c == ':';
			   }) || !std::all_of(Key.begin(), Key.end(), [](unsigned char c) { return c >= 0x21 && c <= 0x7e; }))
				return false;
			std::string Host = Url.substr(6);
			const auto Slash = Host.find('/');
			if(Slash != Host.npos)
			{
				if(Host.substr(Slash) != "/ws")
					return false;
				Host.resize(Slash);
			}
			const auto Colon = Host.find(':');
			if(Colon != Host.npos)
			{
				if(Host.substr(Colon) != ":443")
					return false;
				Host.resize(Colon);
			}
			if(Host.empty() || Host.size() > 253)
				return false;
			size_t Start = 0;
			while(Start < Host.size())
			{
				const auto Dot = Host.find('.', Start);
				const auto End = Dot == Host.npos ? Host.size() : Dot;
				const auto Label = std::string_view(Host).substr(Start, End - Start);
				if(Label.empty() || Label.size() > 63 || Label.front() == '-' || Label.back() == '-' ||
					!std::all_of(Label.begin(), Label.end(), [](unsigned char c) {
						return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
					}))
					return false;
				if(Dot == Host.npos)
					break;
				Start = Dot + 1;
				if(Start == Host.size())
					return false;
			}
			Config.m_Host = std::move(Host);
			Config.m_Room = Room;
			Config.m_Uid = Uid;
			Config.m_Key = Key;
			return true;
		}

		std::string EncodeRoom(std::string_view Room)
		{
			std::string Result;
			constexpr char aHex[] = "0123456789ABCDEF";
			for(unsigned char c : Room)
			{
				if(c == '[' || c == ']' || c == ':')
				{
					Result += '%';
					Result += aHex[c >> 4];
					Result += aHex[c & 15];
				}
				else
					Result += static_cast<char>(c);
			}
			return Result;
		}

		// Split structural separators only; a D payload may itself contain '|'.
		bool ParseFrame(std::string_view Text, CEvent &Event, char &Type)
		{
			if(Text.size() > MAX_FRAME || !IsUtf8(Text))
				return false;
			if(Text == "PONG" || Text == "PING")
			{
				Type = Text == "PONG" ? 'p' : 'q';
				return true;
			}
			if(Text.size() < 2 || Text[1] != '|')
				return false;
			Type = Text[0];
			Text.remove_prefix(2);
			auto Take = [&](std::string_view &Part) {
				const auto Pos = Text.find('|');
				if(Pos == Text.npos)
					return false;
				Part = Text.substr(0, Pos);
				Text.remove_prefix(Pos + 1);
				return true;
			};
			std::string_view First, Second, Third;
			uint64_t Count = 0;
			if(Type == 'D')
			{
				if(!Take(First) || !Take(Second) || !Take(Third) || !IsHexId(First) || !IsHexId(Second) ||
					!Number(Third, Event.m_Sequence) || Event.m_Sequence == 0 || Text.size() > MAX_PAYLOAD || Text.substr(0, 5) != "\xE2\x9F\xA8"
																		       "B:")
					return false;
				Event.m_Type = CEvent::EType::SIGNAL;
				Event.m_Uid = First;
				Event.m_Session = Second;
				Event.m_Payload = Text;
				return true;
			}
			if(Type == 'W' || Type == 'A')
			{
				if(!Take(First) || !Take(Second) || Text.empty() || Text.find('|') != Text.npos)
					return false;
				if(Type == 'W')
				{
					if(!IsHexId(First) || Second.empty() || !Number(Text, Count) || Count == 0)
						return false;
					Event.m_Type = CEvent::EType::CONNECTED;
					Event.m_Uid = First;
					Event.m_Payload = Second; // Server boot id.
				}
				else
				{
					if(!Number(First, Event.m_Sequence) || Event.m_Sequence == 0 || !Number(Second, Count))
						return false;
					Event.m_Type = CEvent::EType::ACK;
					Event.m_Payload = Text; // Server boot id, possibly changed after hibernation.
				}
				if(Count > static_cast<uint64_t>(std::numeric_limits<int>::max()))
					return false;
				Event.m_Recipients = static_cast<int>(Count);
				return true;
			}
			if(Type == 'I')
				return Take(First) && !First.empty() && Number(Text, Count);
			// Do not expose untrusted server error text (it could reflect a credential).
			return Type == 'E' && !Text.empty();
		}

		struct CSend
		{
			std::string m_Payload;
			CClock::time_point m_QueuedAt;
		};

		struct CState
		{
			std::mutex m_Mutex;
			std::condition_variable m_Cv;
			bool m_Quit = false;
			bool m_WorkerAvailable = true;
			uint64_t m_Generation = 0;
			std::shared_ptr<CConfig> m_pConfig;
			CStatus m_Status;
			std::deque<CSend> m_Sends;
			std::deque<std::pair<CClock::time_point, CEvent>> m_Events;

			bool Current(uint64_t Generation) const
			{
				return !m_Quit && m_pConfig && m_Generation == Generation;
			}

			void Expire()
			{
				const auto Now = CClock::now();
				while(!m_Sends.empty() && Now - m_Sends.front().m_QueuedAt > 2s)
					m_Sends.pop_front();
			}

			void Event(CEvent Event)
			{
				// Preserve the newest bounded history, including offline transitions.
				if(m_Events.size() == MAX_EVENTS)
					m_Events.pop_front();
				m_Events.emplace_back(CClock::now(), std::move(Event));
			}
		};

		struct CIo
		{
			std::mutex m_Mutex;
			std::shared_ptr<CState> m_pState;
			// WinHTTP owns this reference until its final HANDLE_CLOSING callback.
			// A delayed callback therefore cannot touch freed receive/send storage.
			std::shared_ptr<CIo> m_pCallbackLifetime;
			bool m_Sent = false;
			bool m_Headers = false;
			bool m_Read = false;
			bool m_Written = false;
			bool m_Shutdown = false;
			bool m_Closed = false;
			bool m_Cancel = false;
			bool m_Welcomed = false;
			bool m_Pong = false;
			bool m_PongNeeded = false;
			DWORD m_Error = 0;
			DWORD m_Bytes = 0;
			WINHTTP_WEB_SOCKET_BUFFER_TYPE m_Kind = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
			std::array<char, MAX_FRAME> m_aBuffer{};
			std::string m_SendBuffer;
			std::wstring m_HeaderBuffer;
			~CIo()
			{
				if(!m_HeaderBuffer.empty())
					SecureZeroMemory(m_HeaderBuffer.data(), m_HeaderBuffer.size() * sizeof(wchar_t));
			}
		};

		void CALLBACK Callback(HINTERNET, DWORD_PTR Context, DWORD Status, LPVOID pInfo, DWORD Length)
		{
			if(!Context)
				return;
			auto *pIo = reinterpret_cast<CIo *>(Context);
			std::shared_ptr<CIo> pLifetime;
			{
				std::lock_guard Lock(pIo->m_Mutex);
				pLifetime = pIo->m_pCallbackLifetime;
				switch(Status)
				{
				case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE: pIo->m_Sent = true; break;
				case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE: pIo->m_Headers = true; break;
				case WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE: pIo->m_Written = true; break;
				case WINHTTP_CALLBACK_STATUS_SHUTDOWN_COMPLETE: pIo->m_Shutdown = true; break;
				case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
					if(pInfo && Length >= sizeof(WINHTTP_WEB_SOCKET_STATUS))
					{
						const auto *pResult = static_cast<WINHTTP_WEB_SOCKET_STATUS *>(pInfo);
						pIo->m_Bytes = pResult->dwBytesTransferred;
						pIo->m_Kind = pResult->eBufferType;
					}
					else
						pIo->m_Error = ERROR_INVALID_DATA;
					pIo->m_Read = true;
					break;
				case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
					pIo->m_Error = pInfo && Length >= sizeof(WINHTTP_ASYNC_RESULT) ? static_cast<WINHTTP_ASYNC_RESULT *>(pInfo)->dwError : ERROR_INVALID_DATA;
					if(!pIo->m_Error)
						pIo->m_Error = ERROR_INVALID_DATA;
					break;
				case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
					pIo->m_Closed = true;
					pIo->m_pCallbackLifetime.reset();
					break;
				default: break;
				}
			}
			// Synchronize notification with the waiter's predicate; never nest io->state locks.
			std::lock_guard Lock(pIo->m_pState->m_Mutex);
			pIo->m_pState->m_Cv.notify_all();
		}

		bool Pending(DWORD Code)
		{
			return Code == ERROR_SUCCESS || Code == ERROR_IO_PENDING;
		}

		bool Wait(const std::shared_ptr<CIo> &pIo, bool CIo::*pFlag, uint64_t Generation, CClock::time_point Deadline)
		{
			auto &State = *pIo->m_pState;
			std::unique_lock Lock(State.m_Mutex);
			State.m_Cv.wait_until(Lock, Deadline, [&] {
				std::lock_guard IoLock(pIo->m_Mutex);
				return !State.Current(Generation) || pIo->m_Cancel || pIo->m_Error || pIo.get()->*pFlag;
			});
			std::lock_guard IoLock(pIo->m_Mutex);
			return State.Current(Generation) && !pIo->m_Cancel && !pIo->m_Error && pIo.get()->*pFlag;
		}

		void CloseIo(HINTERNET Handle, const std::shared_ptr<CIo> &pIo)
		{
			if(!Handle)
				return;
			WinHttpCloseHandle(Handle);
			auto &State = *pIo->m_pState;
			std::unique_lock Lock(State.m_Mutex);
			State.m_Cv.wait_for(Lock, 500ms, [&] {
				std::lock_guard IoLock(pIo->m_Mutex);
				return pIo->m_Closed;
			});
			// If HANDLE_CLOSING is delayed/missing, the callback-owned reference remains.
			// This is a safety fallback, not a detached worker or a reference to CTransport.
		}

		class CConnection
		{
			std::shared_ptr<CState> m_pState;
			std::shared_ptr<CConfig> m_pConfig;
			uint64_t m_Generation;
			HINTERNET m_Session = nullptr;
			HINTERNET m_Connect = nullptr;
			HINTERNET m_Request = nullptr;
			HINTERNET m_Socket = nullptr;
			std::shared_ptr<CIo> m_pRequestIo;
			std::shared_ptr<CIo> m_pIo;
			std::mutex m_Gate;
			std::thread m_Receiver;
			std::string m_SessionId = RandomId();
			std::string m_Error = "connection failed";
			uint64_t m_NextSequence = 1;

			std::shared_ptr<CIo> NewIo()
			{
				auto pIo = std::make_shared<CIo>();
				pIo->m_pState = m_pState;
				return pIo;
			}

			bool Fail(const char *pOperation, DWORD Code = ERROR_INVALID_DATA)
			{
				m_Error = std::string(pOperation) + " (" + std::to_string(Code) + ")";
				return false;
			}

			void ReceiverFailure(const char *pReason)
			{
				std::lock_guard Lock(m_pState->m_Mutex);
				if(m_pState->Current(m_Generation))
				{
					m_pState->m_Status.m_Online = false;
					m_pState->m_Status.m_Error = pReason;
					m_pState->m_Sends.clear();
				}
				{
					std::lock_guard IoLock(m_pIo->m_Mutex);
					if(!m_pIo->m_Error)
						m_pIo->m_Error = ERROR_INVALID_DATA;
				}
				m_pState->m_Cv.notify_all();
			}

			void Receive()
			{
				std::string Text;
				std::map<std::pair<std::string, std::string>, uint64_t> LastReceived;
				bool Welcomed = false;
				const auto WelcomeDeadline = CClock::now() + 10s;
				for(;;)
				{
					DWORD Code;
					{
						std::lock_guard Gate(m_Gate);
						{
							std::lock_guard IoLock(m_pIo->m_Mutex);
							if(m_pIo->m_Cancel || !m_Socket)
								return;
							m_pIo->m_Read = false;
						}
						Code = WinHttpWebSocketReceive(m_Socket, m_pIo->m_aBuffer.data(), static_cast<DWORD>(m_pIo->m_aBuffer.size()), nullptr, nullptr);
					}
					if(!Pending(Code) || !Wait(m_pIo, &CIo::m_Read, m_Generation, Welcomed ? CClock::time_point::max() : WelcomeDeadline))
					{
						ReceiverFailure(Welcomed ? "receive failed" : "welcome failed");
						return;
					}
					DWORD Bytes;
					WINHTTP_WEB_SOCKET_BUFFER_TYPE Kind;
					{
						std::lock_guard IoLock(m_pIo->m_Mutex);
						Bytes = m_pIo->m_Bytes;
						Kind = m_pIo->m_Kind;
					}
					if(Kind == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
					{
						ReceiverFailure("server closed connection");
						return;
					}
					if((Kind != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE && Kind != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) ||
						Bytes > MAX_FRAME || Text.size() + Bytes > MAX_FRAME)
					{
						ReceiverFailure("invalid or oversized websocket frame");
						return;
					}
					Text.append(m_pIo->m_aBuffer.data(), Bytes);
					if(Kind == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
						continue;
					CEvent Event;
					char Type = 0;
					if(!ParseFrame(Text, Event, Type))
					{
						ReceiverFailure("invalid relay protocol");
						return;
					}
					Text.clear();
					if(Type == 'D')
					{
						const auto Identity = std::make_pair(Event.m_Uid, Event.m_Session);
						auto Previous = LastReceived.find(Identity);
						if(Previous != LastReceived.end() && Event.m_Sequence <= Previous->second)
							continue;
						if(Previous == LastReceived.end() && LastReceived.size() >= MAX_EVENTS)
						{
							ReceiverFailure("too many sender sessions");
							return;
						}
						LastReceived[Identity] = Event.m_Sequence;
					}
					if(Type == 'E' || (Type == 'W' && (Welcomed || Event.m_Uid != m_pConfig->m_Uid)) || (!Welcomed && Type != 'W'))
					{
						ReceiverFailure(Type == 'E' ? "relay rejected signal" : "unexpected relay welcome");
						return;
					}
					std::lock_guard Lock(m_pState->m_Mutex);
					if(!m_pState->Current(m_Generation))
						return;
					if(Type == 'W')
					{
						Welcomed = true;
						Event.m_Session = m_SessionId;
						m_pState->m_Status.m_Online = true;
						m_pState->m_Status.m_Error.clear();
						std::lock_guard IoLock(m_pIo->m_Mutex);
						m_pIo->m_Welcomed = true;
					}
					if(Type == 'A')
					{
						Event.m_Uid = m_pConfig->m_Uid;
						Event.m_Session = m_SessionId;
					}
					if(Type == 'p' || Type == 'q')
					{
						std::lock_guard IoLock(m_pIo->m_Mutex);
						if(Type == 'p')
							m_pIo->m_Pong = true;
						else
							m_pIo->m_PongNeeded = true;
					}
					if(Type == 'W' || Type == 'D' || Type == 'A')
						m_pState->Event(std::move(Event));
					m_pState->m_Cv.notify_all();
				}
			}

			bool Write(std::string Text)
			{
				DWORD Code;
				{
					std::lock_guard Gate(m_Gate);
					{
						std::lock_guard IoLock(m_pIo->m_Mutex);
						if(m_pIo->m_Cancel || m_pIo->m_Error || !m_Socket)
							return false;
						m_pIo->m_Written = false;
						m_pIo->m_SendBuffer = std::move(Text);
					}
					Code = WinHttpWebSocketSend(m_Socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, m_pIo->m_SendBuffer.data(), static_cast<DWORD>(m_pIo->m_SendBuffer.size()));
				}
				return Pending(Code) && Wait(m_pIo, &CIo::m_Written, m_Generation, CClock::now() + 5s);
			}

		public:
			CConnection(std::shared_ptr<CState> pState, std::shared_ptr<CConfig> pConfig, uint64_t Generation) :
				m_pState(std::move(pState)), m_pConfig(std::move(pConfig)), m_Generation(Generation), m_pRequestIo(NewIo()), m_pIo(NewIo())
			{
			}

			~CConnection()
			{
				// Only the worker reaches here. Main-thread Stop just changes generation.
				HINTERNET Socket;
				{
					std::lock_guard Gate(m_Gate);
					Socket = std::exchange(m_Socket, nullptr);
					{
						std::lock_guard IoLock(m_pIo->m_Mutex);
						m_pIo->m_Cancel = true;
					}
					if(Socket)
						WinHttpWebSocketShutdown(Socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
				}
				{
					std::lock_guard Lock(m_pState->m_Mutex);
					m_pState->m_Cv.notify_all();
				}
				CloseIo(Socket, m_pIo);
				if(m_Receiver.joinable())
					m_Receiver.join();
				CloseIo(std::exchange(m_Request, nullptr), m_pRequestIo);
				if(m_Connect)
					WinHttpCloseHandle(m_Connect);
				if(m_Session)
					WinHttpCloseHandle(m_Session);
			}

			bool Connect()
			{
				m_Session = WinHttpOpen(L"TClient-Relay/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
				if(!m_Session)
					return Fail("open session", GetLastError());
				// Normal system TLS/host validation. No security, proxy or certificate override.
				if(!WinHttpSetTimeouts(m_Session, 5000, 5000, 5000, 5000))
					return Fail("configure timeouts", GetLastError());
				m_Connect = WinHttpConnect(m_Session, Wide(m_pConfig->m_Host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
				if(!m_Connect)
					return Fail("open connection", GetLastError());
				const auto Path = Wide("/ws?room=" + EncodeRoom(m_pConfig->m_Room) + "&uid=" + m_pConfig->m_Uid + "&session=" + m_SessionId);
				m_Request = WinHttpOpenRequest(m_Connect, L"GET", Path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
				if(!m_Request)
					return Fail("open request", GetLastError());
				DWORD_PTR Context = reinterpret_cast<DWORD_PTR>(m_pRequestIo.get());
				if(!WinHttpSetOption(m_Request, WINHTTP_OPTION_CONTEXT_VALUE, &Context, sizeof(Context)))
					return Fail("configure context", GetLastError());
				m_pRequestIo->m_pCallbackLifetime = m_pRequestIo;
				if(WinHttpSetStatusCallback(m_Request, Callback, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, 0) == WINHTTP_INVALID_STATUS_CALLBACK)
				{
					m_pRequestIo->m_pCallbackLifetime.reset();
					return Fail("configure callback", GetLastError());
				}
				DWORD Disable = WINHTTP_DISABLE_REDIRECTS;
				if(!WinHttpSetOption(m_Request, WINHTTP_OPTION_DISABLE_FEATURE, &Disable, sizeof(Disable)) ||
					!WinHttpSetOption(m_Request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
					return Fail("configure upgrade", GetLastError());
				auto &Header = m_pRequestIo->m_HeaderBuffer;
				if(!m_pConfig->m_Key.empty())
				{
					Header = L"X-Prototype-Key: ";
					Header.append(m_pConfig->m_Key.begin(), m_pConfig->m_Key.end());
					Header += L"\r\n";
				}
				const BOOL Sent = WinHttpSendRequest(m_Request, Header.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : Header.c_str(), static_cast<DWORD>(Header.size()), WINHTTP_NO_REQUEST_DATA, 0, 0, Context);
				DWORD Code = Sent ? ERROR_SUCCESS : GetLastError();
				const auto Deadline = CClock::now() + 12s;
				if(!Pending(Code) || !Wait(m_pRequestIo, &CIo::m_Sent, m_Generation, Deadline))
					return Fail("send upgrade", Code ? Code : ERROR_WINHTTP_TIMEOUT);
				const BOOL Received = WinHttpReceiveResponse(m_Request, nullptr);
				Code = Received ? ERROR_SUCCESS : GetLastError();
				if(!Pending(Code) || !Wait(m_pRequestIo, &CIo::m_Headers, m_Generation, Deadline))
					return Fail("receive upgrade", Code ? Code : ERROR_WINHTTP_TIMEOUT);
				DWORD Status = 0, Size = sizeof(Status);
				if(!WinHttpQueryHeaders(m_Request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &Status, &Size, WINHTTP_NO_HEADER_INDEX))
					return Fail("query upgrade status", GetLastError());
				if(Status != 101)
					return Fail("upgrade HTTP status", Status);
				// Set request timeouts before upgrade: WebSocket handles do not accept SetTimeouts.
				if(!WinHttpSetTimeouts(m_Request, 5000, 5000, 5000, 120000))
					return Fail("configure websocket timeouts", GetLastError());
				m_pIo->m_pCallbackLifetime = m_pIo;
				m_Socket = WinHttpWebSocketCompleteUpgrade(m_Request, reinterpret_cast<DWORD_PTR>(m_pIo.get()));
				if(!m_Socket)
				{
					m_pIo->m_pCallbackLifetime.reset();
					return Fail("complete upgrade", GetLastError());
				}
				CloseIo(std::exchange(m_Request, nullptr), m_pRequestIo);
				DWORD Keepalive = 60000, CloseTimeout = 500;
				if(!WinHttpSetOption(m_Socket, WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL, &Keepalive, sizeof(Keepalive)) ||
					!WinHttpSetOption(m_Socket, WINHTTP_OPTION_WEB_SOCKET_CLOSE_TIMEOUT, &CloseTimeout, sizeof(CloseTimeout)))
					return Fail("configure websocket", GetLastError());
				m_Receiver = std::thread([this] {
					try
					{
						Receive();
					}
					catch(...)
					{
						ReceiverFailure("receive resource failure");
					}
				});
				return Wait(m_pIo, &CIo::m_Welcomed, m_Generation, CClock::now() + 10s) || Fail("welcome", ERROR_WINHTTP_TIMEOUT);
			}

			void Run()
			{
				auto Heartbeat = CClock::now() + 20s;
				auto PongDeadline = CClock::time_point::max();
				for(;;)
				{
					std::string Text;
					{
						std::unique_lock Lock(m_pState->m_Mutex);
						m_pState->m_Cv.wait_until(Lock, std::min(Heartbeat, PongDeadline), [&] {
							std::lock_guard IoLock(m_pIo->m_Mutex);
							return !m_pState->Current(m_Generation) || m_pIo->m_Error || m_pIo->m_Pong || m_pIo->m_PongNeeded || !m_pState->m_Sends.empty();
						});
						if(!m_pState->Current(m_Generation))
							return;
						{
							std::lock_guard IoLock(m_pIo->m_Mutex);
							if(m_pIo->m_Error)
							{
								m_Error = m_pState->m_Status.m_Error.empty() ? "websocket I/O failed" : m_pState->m_Status.m_Error;
								return;
							}
							if(m_pIo->m_Pong)
							{
								m_pIo->m_Pong = false;
								PongDeadline = CClock::time_point::max();
							}
							if(m_pIo->m_PongNeeded)
							{
								m_pIo->m_PongNeeded = false;
								Text = "PONG";
							}
						}
						const auto Now = CClock::now();
						if(Now >= PongDeadline)
						{
							Fail("heartbeat timeout", ERROR_WINHTTP_TIMEOUT);
							return;
						}
						if(Text.empty() && Now >= Heartbeat)
						{
							Text = "PING"; // Server WebSocket auto-response: does not wake the DO.
							Heartbeat = Now + 20s;
							PongDeadline = Now + 10s;
						}
						m_pState->Expire();
						if(Text.empty() && !m_pState->m_Sends.empty())
						{
							if(m_NextSequence == std::numeric_limits<uint64_t>::max())
							{
								Fail("sequence exhausted");
								return;
							}
							Text = "S|" + std::to_string(m_NextSequence++) + "|" + m_pState->m_Sends.front().m_Payload;
							m_pState->m_Sends.pop_front();
						}
					}
					if(!Text.empty() && !Write(std::move(Text)))
					{
						Fail("send failed", ERROR_WINHTTP_TIMEOUT);
						return;
					}
				}
			}

			const std::string &Error() const { return m_Error; }
			const std::string &SessionId() const { return m_SessionId; }
			bool Welcomed() const
			{
				std::lock_guard Lock(m_pIo->m_Mutex);
				return m_pIo->m_Welcomed;
			}
		};
	} // namespace

	class CTransport::CImpl
	{
	public:
		std::shared_ptr<CState> m_pState = std::make_shared<CState>();
		std::thread m_Worker;

		CImpl()
		{
			try
			{
				m_Worker = std::thread([pState = m_pState] {
					try
					{
						Worker(pState);
					}
					catch(...)
					{
						std::lock_guard Lock(pState->m_Mutex);
						pState->m_WorkerAvailable = false;
						pState->m_Status.m_Online = false;
						pState->m_Status.m_Error = "transport resource failure";
						pState->m_Sends.clear();
					}
				});
			}
			catch(...)
			{
				m_pState->m_WorkerAvailable = false;
				m_pState->m_Status.m_Error = "worker unavailable";
			}
		}

		~CImpl()
		{
			{
				std::lock_guard Lock(m_pState->m_Mutex);
				m_pState->m_Quit = true;
				++m_pState->m_Generation;
				m_pState->m_pConfig.reset();
				m_pState->m_Sends.clear();
				m_pState->m_Events.clear();
				m_pState->m_Cv.notify_all();
			}
			if(m_Worker.joinable())
				m_Worker.join();
		}

		static void Worker(const std::shared_ptr<CState> &pState)
		{
			uint64_t LastGeneration = 0;
			unsigned Backoff = 1;
			for(;;)
			{
				std::shared_ptr<CConfig> pConfig;
				uint64_t Generation;
				{
					std::unique_lock Lock(pState->m_Mutex);
					pState->m_Cv.wait(Lock, [&] { return pState->m_Quit || pState->m_pConfig; });
					if(pState->m_Quit)
						return;
					pConfig = pState->m_pConfig;
					Generation = pState->m_Generation;
					if(Generation != LastGeneration)
					{
						Backoff = 1;
						LastGeneration = Generation;
					}
				}
				bool Connected = false;
				{
					CConnection Connection(pState, pConfig, Generation);
					Connected = Connection.Connect();
					const auto ConnectedAt = CClock::now();
					if(Connected)
						Connection.Run();
					if(Connected && CClock::now() - ConnectedAt >= 30s)
						Backoff = 1;
					std::lock_guard Lock(pState->m_Mutex);
					if(pState->Current(Generation))
					{
						pState->m_Status.m_Online = false;
						pState->m_Status.m_Error = Connection.Error();
						pState->m_Sends.clear(); // Never replay an uncertain send after reconnect.
						if(Connection.Welcomed())
						{
							CEvent Event;
							Event.m_Type = CEvent::EType::DISCONNECTED;
							Event.m_Uid = pConfig->m_Uid;
							Event.m_Session = Connection.SessionId();
							Event.m_Payload = pState->m_Status.m_Error;
							pState->Event(std::move(Event));
						}
					}
				}
				// 1/2/4/8/16/30 seconds, plus up to 20% positive jitter, capped at 30s.
				const auto Delay = std::min(30000u, Backoff * 1000u + static_cast<unsigned>(secure_rand_below(static_cast<int>(Backoff * 200u + 1u))));
				Backoff = std::min(30u, Backoff * 2u);
				std::unique_lock Lock(pState->m_Mutex);
				pState->m_Cv.wait_for(Lock, std::chrono::milliseconds(Delay), [&] { return !pState->Current(Generation); });
			}
		}
	};

	CTransport::CTransport() :
		m_pImpl(std::make_unique<CImpl>()) {}
	CTransport::~CTransport() = default;

	void CTransport::Start(const std::string &Url, const std::string &Room, const std::string &Uid, const std::string &Key)
	{
		auto pConfig = std::make_shared<CConfig>();
		const bool Valid = Configure(Url, Room, Uid, Key, *pConfig);
		std::lock_guard Lock(m_pImpl->m_pState->m_Mutex);
		auto &State = *m_pImpl->m_pState;
		++State.m_Generation;
		State.m_Status = {};
		State.m_Sends.clear();
		State.m_Events.clear();
		State.m_pConfig.reset();
		if(!Valid)
			State.m_Status.m_Error = "invalid relay configuration";
		else if(!State.m_WorkerAvailable)
			State.m_Status.m_Error = "worker unavailable";
		else
			State.m_pConfig = std::move(pConfig);
		State.m_Cv.notify_all();
	}

	void CTransport::Stop()
	{
		std::lock_guard Lock(m_pImpl->m_pState->m_Mutex);
		auto &State = *m_pImpl->m_pState;
		++State.m_Generation;
		State.m_pConfig.reset();
		State.m_Status = {};
		State.m_Sends.clear();
		State.m_Events.clear();
		State.m_Cv.notify_all();
	}

	bool CTransport::Send(const char *pPayload)
	{
		if(!pPayload)
			return false;
		size_t Length = 0;
		while(Length <= MAX_PAYLOAD && pPayload[Length])
			++Length;
		if(Length == 0 || Length > MAX_PAYLOAD || std::string_view(pPayload, Length).substr(0, 5) != "\xE2\x9F\xA8"
													     "B:" ||
			!IsUtf8(std::string_view(pPayload, Length)))
			return false;
		std::lock_guard Lock(m_pImpl->m_pState->m_Mutex);
		auto &State = *m_pImpl->m_pState;
		State.Expire();
		if(!State.m_Status.m_Online || !State.m_pConfig || State.m_Sends.size() >= MAX_SENDS)
			return false;
		State.m_Sends.push_back({std::string(pPayload, Length), CClock::now()});
		State.m_Cv.notify_all();
		return true;
	}

	std::vector<CEvent> CTransport::Drain()
	{
		std::lock_guard Lock(m_pImpl->m_pState->m_Mutex);
		auto &Events = m_pImpl->m_pState->m_Events;
		std::vector<CEvent> Result;
		Result.reserve(Events.size());
		while(!Events.empty())
		{
			if(Events.front().second.m_Type != CEvent::EType::SIGNAL || CClock::now() - Events.front().first <= 2s)
				Result.push_back(std::move(Events.front().second));
			Events.pop_front();
		}
		return Result;
	}

	CStatus CTransport::Status() const
	{
		std::lock_guard Lock(m_pImpl->m_pState->m_Mutex);
		auto &State = *m_pImpl->m_pState;
		State.Expire();
		auto Result = State.m_Status;
		Result.m_Queued = State.m_Sends.size();
		return Result;
	}
} // namespace RelayWs

#else

namespace RelayWs
{
	class CTransport::CImpl
	{
	};
	CTransport::CTransport() :
		m_pImpl(std::make_unique<CImpl>()) {}
	CTransport::~CTransport() = default;
	void CTransport::Start(const std::string &, const std::string &, const std::string &, const std::string &) {}
	void CTransport::Stop() {}
	bool CTransport::Send(const char *) { return false; }
	std::vector<CEvent> CTransport::Drain() { return {}; }
	CStatus CTransport::Status() const { return {false, 0, "not supported"}; }
} // namespace RelayWs

#endif
