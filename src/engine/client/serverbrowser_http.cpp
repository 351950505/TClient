#include "serverbrowser_http.h"

#include <base/fs.h>
#include <base/hash.h>
#include <base/io.h>
#include <base/log.h>
#include <base/net.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/engine.h>
#include <engine/http.h>
#include <engine/serverbrowser.h>
#include <engine/shared/jobs.h>
#include <engine/shared/json.h>
#include <engine/shared/linereader.h>
#include <engine/shared/serverinfo.h>
#include <engine/storage.h>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
	constexpr size_t MAX_URLS = 16;
	constexpr size_t MAX_RESPONSE_SIZE = 8 * 1024 * 1024;
	constexpr int MAX_CACHE_AGE = 24 * 60 * 60;
	constexpr int FRESH_SERVERLIST_AGE = 5 * 60;
	constexpr size_t MAX_CONCURRENT_REQUESTS = 2;
	constexpr const char *CACHE_PATH = "cache/tclient-serverlist.json";

	bool ParseServerList(json_value *pJson, std::vector<CServerInfo> *pvServers)
	{
		if(pJson == nullptr)
			return true;

		const json_value &Servers = (*pJson)["servers"];
		if(Servers.type != json_array)
			return true;

		std::vector<CServerInfo> vServers;
		for(unsigned int i = 0; i < Servers.u.array.length; i++)
		{
			const json_value &Server = Servers[i];
			const json_value &Addresses = Server["addresses"];
			const json_value &Info = Server["info"];
			const json_value &Location = Server["location"];
			int ParsedLocation = CServerInfo::LOC_UNKNOWN;
			CServerInfo2 ParsedInfo;
			if(Addresses.type != json_array || (Location.type != json_string && Location.type != json_none))
				return true;
			if(Location.type == json_string && CServerInfo::ParseLocation(&ParsedLocation, Location))
				return true;
			if(CServerInfo2::FromJson(&ParsedInfo, &Info))
			{
				// Server info is user input. Skip only the malformed server instead of
				// discarding an otherwise valid master response.
				continue;
			}

			CServerInfo Parsed = ParsedInfo;
			Parsed.m_Location = ParsedLocation;
			Parsed.m_NumAddresses = 0;
			bool HasSix = false;
			for(unsigned int AddressIndex = 0; AddressIndex < Addresses.u.array.length; AddressIndex++)
			{
				if(Addresses[AddressIndex].type != json_string)
					return true;
				if(str_startswith(Addresses[AddressIndex], "tw-0.6+udp://"))
					HasSix = true;
			}
			for(unsigned int AddressIndex = 0; AddressIndex < Addresses.u.array.length; AddressIndex++)
			{
				const char *pAddress = Addresses[AddressIndex].u.string.ptr;
				if(HasSix && str_startswith(pAddress, "tw-0.7+udp://"))
					continue;
				NETADDR ParsedAddress;
				if(net_addr_from_url(&ParsedAddress, pAddress, nullptr, 0) || ParsedAddress.port == 0)
					continue;
				if(Parsed.m_NumAddresses < (int)std::size(Parsed.m_aAddresses))
					Parsed.m_aAddresses[Parsed.m_NumAddresses++] = ParsedAddress;
			}
			if(Parsed.m_NumAddresses > 0)
				vServers.push_back(Parsed);
		}

		*pvServers = std::move(vServers);
		return false;
	}

	struct CServerListSaveState
	{
		std::mutex m_Lock;
		std::string m_Path;
		std::string m_Pending;
		bool m_WorkerActive = false;
	};

	bool WriteServerListCache(const std::string &Path, const std::string &Body)
	{
		char aTmp[IO_MAX_PATH_LENGTH];
		IStorage::FormatTmpPath(aTmp, sizeof(aTmp), Path.c_str());
		IOHANDLE File = io_open(aTmp, IOFLAG_WRITE);
		if(!File)
			return false;
		const bool Written = io_write(File, Body.data(), Body.size()) == Body.size();
		const int Closed = io_close(File);
		if(!Written || Closed != 0 || fs_rename(aTmp, Path.c_str()) != 0)
		{
			(void)fs_remove(aTmp);
			return false;
		}
		return true;
	}

	class CServerListSaveJob : public IJob
	{
		std::shared_ptr<CServerListSaveState> m_pState;

		void Run() override
		{
			while(true)
			{
				std::string Path;
				std::string Body;
				{
					std::lock_guard Lock(m_pState->m_Lock);
					if(m_pState->m_Pending.empty())
					{
						m_pState->m_WorkerActive = false;
						return;
					}
					Path = m_pState->m_Path;
					Body = std::move(m_pState->m_Pending);
					m_pState->m_Pending.clear();
				}
				if(!WriteServerListCache(Path, Body))
					log_error("serverbrowser_http", "failed to persist serverlist cache");
			}
		}

	public:
		explicit CServerListSaveJob(std::shared_ptr<CServerListSaveState> pState) :
			m_pState(std::move(pState))
		{
		}
	};

	class CServerBrowserHttp : public IServerBrowserHttp
	{
		struct CAttempt
		{
			int m_SourceIndex;
			std::shared_ptr<IHttpRequest> m_pRequest;
		};

		struct CResponse
		{
			int m_SourceIndex;
			int m_Age;
			std::string m_Body;
			std::vector<CServerInfo> m_vServers;
		};

		IEngine *m_pEngine;
		IStorage *m_pStorage;
		IHttp *m_pHttp;
		std::vector<std::string> m_vUrls;
		std::function<std::shared_ptr<IHttpRequest>(const char *)> m_RequestFactory;
		std::function<int64_t()> m_Clock;
		std::deque<int> m_Queue;
		std::vector<CAttempt> m_vActive;
		std::vector<CServerInfo> m_vServers;
		std::string m_BestUrl;
		std::string m_SourceHash;
		std::shared_ptr<CServerListSaveState> m_pSaveState;
		uint64_t m_Revision = 0;
		int64_t m_ReceivedAt = 0;
		int64_t m_RetryAt = 0;
		int m_SourceAge = 0;
		int m_Failures = 0;
		bool m_Running = false;
		bool m_HadSuccess = false;
		bool m_Error = false;
		bool m_Shutdown = false;

		int64_t CurrentAge() const
		{
			return (int64_t)m_SourceAge + std::max<int64_t>(0, m_Clock() - m_ReceivedAt);
		}

		void Cancel()
		{
			for(auto &Attempt : m_vActive)
				if(Attempt.m_pRequest)
					Attempt.m_pRequest->Abort();
			m_vActive.clear();
			m_Queue.clear();
			m_Running = false;
		}

		void Fill()
		{
			while(m_vActive.size() < MAX_CONCURRENT_REQUESTS && !m_Queue.empty())
			{
				const int SourceIndex = m_Queue.front();
				m_Queue.pop_front();
				std::shared_ptr<IHttpRequest> pRequest = m_RequestFactory(m_vUrls[SourceIndex].c_str());
				if(!pRequest)
					continue;
				pRequest->Timeout(CTimeout{6000, 30000, 1024, 10});
				pRequest->MaxResponseSize(MAX_RESPONSE_SIZE);
				pRequest->LogProgress(HTTPLOG::FAILURE);
				log_info("serverbrowser_http", "fetching serverlist, url='%s'", m_vUrls[SourceIndex].c_str());
				m_pHttp->Run(pRequest);
				m_vActive.push_back({SourceIndex, std::move(pRequest)});
			}
		}

		void Publish(std::vector<CServerInfo> vServers, int Age, int SourceIndex, const std::string &Body)
		{
			m_vServers = std::move(vServers);
			m_SourceAge = Age;
			m_ReceivedAt = m_Clock();
			m_BestUrl = m_vUrls[SourceIndex];
			m_Error = false;
			++m_Revision;
			Save(Body, Age);
			log_info("serverbrowser_http", "published serverlist, url='%s' age=%ds", m_BestUrl.c_str(), Age);
		}

		std::optional<CResponse> ProcessResponse(int SourceIndex, const std::shared_ptr<IHttpRequest> &pRequest)
		{
			if(pRequest->State() != EHttpState::DONE)
				return std::nullopt;

			json_value *pJson = pRequest->ResultJson();
			std::vector<CServerInfo> vServers;
			const bool Valid = pJson != nullptr && !ParseServerList(pJson, &vServers) && !vServers.empty();
			json_value_free(pJson);
			if(!Valid)
				return std::nullopt;

			const std::optional<int64_t> ResponseAge = pRequest->ResultAgeSeconds();
			int Age = FRESH_SERVERLIST_AGE + 1;
			if(ResponseAge.has_value())
			{
				if(*ResponseAge < 0 || *ResponseAge > MAX_CACHE_AGE)
					return std::nullopt;
				Age = (int)*ResponseAge;
			}

			unsigned char *pBody = nullptr;
			size_t BodySize = 0;
			pRequest->Result(&pBody, &BodySize);
			if(pBody == nullptr || BodySize == 0 || BodySize > MAX_RESPONSE_SIZE)
				return std::nullopt;

			m_HadSuccess = true;
			m_Failures = 0;
			m_Error = false;
			return CResponse{
				SourceIndex,
				Age,
				std::string(reinterpret_cast<const char *>(pBody), BodySize),
				std::move(vServers)};
		}

		void Save(const std::string &Body, int Age)
		{
			if(m_pStorage == nullptr || Body.empty() || Body.size() > MAX_RESPONSE_SIZE)
				return;
			m_pStorage->CreateFolder("cache", IStorage::TYPE_SAVE);
			char aPath[IO_MAX_PATH_LENGTH];
			m_pStorage->GetCompletePath(IStorage::TYPE_SAVE, CACHE_PATH, aPath, sizeof(aPath));
			char aHeader[160];
			str_format(aHeader, sizeof(aHeader), "TCSL1 %lld %d %s\n", (long long)time_timestamp(), Age, m_SourceHash.c_str());
			bool StartWorker = false;
			{
				std::lock_guard Lock(m_pSaveState->m_Lock);
				m_pSaveState->m_Path = aPath;
				m_pSaveState->m_Pending = std::string(aHeader) + Body;
				if(!m_pSaveState->m_WorkerActive)
				{
					m_pSaveState->m_WorkerActive = true;
					StartWorker = true;
				}
			}
			if(StartWorker)
				m_pEngine->AddJob(std::make_shared<CServerListSaveJob>(m_pSaveState));
		}

		void Load()
		{
			if(m_pStorage == nullptr)
				return;
			IOHANDLE File = m_pStorage->OpenFile(CACHE_PATH, IOFLAG_READ, IStorage::TYPE_SAVE);
			if(!File)
				return;
			const int64_t Length = io_length(File);
			if(Length <= 0 || Length > (int64_t)MAX_RESPONSE_SIZE + 160)
			{
				io_close(File);
				return;
			}
			std::string Data((size_t)Length, '\0');
			const bool Read = io_read(File, Data.data(), Data.size()) == Data.size();
			io_close(File);
			const size_t HeaderEnd = Data.find('\n');
			if(!Read || HeaderEnd == std::string::npos)
				return;

			long long Received;
			int Age;
			char aHash[SHA256_MAXSTRSIZE];
			const std::string Header = Data.substr(0, HeaderEnd);
			if(sscanf(Header.c_str(), "TCSL1 %lld %d %64s", &Received, &Age, aHash) != 3 || m_SourceHash != aHash || Age < 0 || Age > MAX_CACHE_AGE)
				return;
			const int64_t Now = time_timestamp();
			if(Received < 0 || Received > Now)
				return;
			const int64_t Elapsed = Now - Received;
			if(Elapsed + Age > MAX_CACHE_AGE)
				return;

			json_value *pJson = JsonParse(Data.data() + HeaderEnd + 1, Data.size() - HeaderEnd - 1);
			std::vector<CServerInfo> vServers;
			const bool Valid = pJson != nullptr && !ParseServerList(pJson, &vServers) && !vServers.empty();
			json_value_free(pJson);
			if(!Valid)
				return;

			m_vServers = std::move(vServers);
			m_SourceAge = (int)(Age + Elapsed);
			m_ReceivedAt = m_Clock();
			++m_Revision;
			log_info("serverbrowser_http", "loaded cached serverlist; refreshing in background");
		}

	public:
		CServerBrowserHttp(IEngine *pEngine, IStorage *pStorage, IHttp *pHttp, std::vector<std::string> vUrls, const char *pPreviousBestUrl,
			std::function<std::shared_ptr<IHttpRequest>(const char *)> RequestFactory, std::function<int64_t()> Clock) :
			m_pEngine(pEngine),
			m_pStorage(pStorage),
			m_pHttp(pHttp),
			m_vUrls(std::move(vUrls)),
			m_RequestFactory(std::move(RequestFactory)),
			m_Clock(std::move(Clock)),
			m_pSaveState(std::make_shared<CServerListSaveState>())
		{
			if(m_vUrls.size() > MAX_URLS)
				m_vUrls.resize(MAX_URLS);
			std::string SourceKey;
			for(const std::string &Url : m_vUrls)
				SourceKey += Url + "\n";
			char aHash[SHA256_MAXSTRSIZE];
			sha256_str(sha256(SourceKey.data(), SourceKey.size()), aHash, sizeof(aHash));
			m_SourceHash = aHash;

			const auto Previous = std::find(m_vUrls.begin(), m_vUrls.end(), pPreviousBestUrl == nullptr ? "" : pPreviousBestUrl);
			if(Previous != m_vUrls.end())
				std::rotate(m_vUrls.begin(), Previous, Previous + 1);
			Load();
			Refresh();
		}

		~CServerBrowserHttp() override
		{
			Shutdown();
		}

		void Shutdown() override
		{
			if(m_Shutdown)
				return;
			m_Shutdown = true;
			Cancel();
		}

		void Update() override
		{
			if(m_Shutdown)
				return;
			if(!m_Running)
			{
				if(m_RetryAt != 0 && m_Clock() >= m_RetryAt)
					Refresh();
				return;
			}

			std::vector<CResponse> vResponses;
			for(size_t Index = 0; Index < m_vActive.size();)
			{
				const CAttempt Attempt = m_vActive[Index];
				if(!Attempt.m_pRequest->Done())
				{
					Index++;
					continue;
				}
				m_vActive.erase(m_vActive.begin() + Index);
				std::optional<CResponse> Response = ProcessResponse(Attempt.m_SourceIndex, Attempt.m_pRequest);
				if(Response.has_value())
					vResponses.push_back(std::move(*Response));
			}

			if(!vResponses.empty())
			{
				auto Best = std::min_element(vResponses.begin(), vResponses.end(), [](const CResponse &Left, const CResponse &Right) {
					return Left.m_Age < Right.m_Age;
				});
				if(m_vServers.empty() || Best->m_Age <= CurrentAge())
				{
					const int Age = Best->m_Age;
					Publish(std::move(Best->m_vServers), Age, Best->m_SourceIndex, Best->m_Body);
					if(Age <= FRESH_SERVERLIST_AGE)
					{
						m_RetryAt = 0;
						Cancel();
						return;
					}
				}
				else
				{
					log_info("serverbrowser_http", "retained fresher cached serverlist, response age=%ds", Best->m_Age);
				}
			}

			Fill();
			if(m_vActive.empty() && m_Queue.empty())
			{
				m_Running = false;
				if(!m_HadSuccess)
				{
					m_Error = true;
					m_Failures = std::min(m_Failures + 1, 5);
				}
				const int RetryDelay = m_HadSuccess ? 60 : std::min(60, 5 << (m_Failures - 1));
				m_RetryAt = m_Clock() + RetryDelay;
				log_info("serverbrowser_http", "serverlist refresh %s; retry in %ds", m_HadSuccess ? "stale" : "failed", RetryDelay);
			}
		}

		uint64_t Revision() const override { return m_Revision; }
		bool IsRefreshing() const override { return m_Running; }
		bool IsError() const override { return m_Error; }

		void Refresh() override
		{
			if(m_Shutdown || m_Running)
				return;
			m_Running = true;
			m_HadSuccess = false;
			m_Error = false;
			m_RetryAt = 0;
			m_Queue.clear();
			for(size_t Index = 0; Index < m_vUrls.size(); Index++)
				m_Queue.push_back((int)Index);
			Fill();
			if(m_vActive.empty() && m_Queue.empty())
			{
				m_Running = false;
				m_Error = true;
			}
		}

		bool GetBestUrl(const char **ppBestUrl) const override
		{
			*ppBestUrl = m_BestUrl.empty() ? nullptr : m_BestUrl.c_str();
			return m_BestUrl.empty();
		}

		int NumServers() const override { return (int)m_vServers.size(); }
		const CServerInfo &Server(int Index) const override { return m_vServers[Index]; }
	};

	const char *DEFAULT_SERVERLIST_URLS[] = {
		"https://master1.ddnet.org/ddnet/15/servers.json",
		"https://master2.ddnet.org/ddnet/15/servers.json",
		"https://master3.ddnet.org/ddnet/15/servers.json",
		"https://master4.ddnet.org/ddnet/15/servers.json",
	};
} // namespace

IServerBrowserHttp *CreateServerBrowserHttpWithSources(IEngine *pEngine, IStorage *pStorage, IHttp *pHttp,
	std::vector<std::string> vUrls, const char *pPreviousBestUrl,
	std::function<std::shared_ptr<IHttpRequest>(const char *)> RequestFactory,
	std::function<int64_t()> Clock)
{
	return new CServerBrowserHttp(pEngine, pStorage, pHttp, std::move(vUrls), pPreviousBestUrl, std::move(RequestFactory), std::move(Clock));
}

IServerBrowserHttp *CreateServerBrowserHttp(IEngine *pEngine, IStorage *pStorage, IHttp *pHttp, const char *pPreviousBestUrl)
{
	std::vector<std::string> vUrls;
	CLineReader LineReader;
	if(LineReader.OpenFile(pStorage->OpenFile("ddnet-serverlist-urls.cfg", IOFLAG_READ, IStorage::TYPE_ALL)))
	{
		while(const char *pLine = LineReader.Get())
		{
			if(vUrls.size() == MAX_URLS)
				break;
			if(*pLine != '\0' && std::find(vUrls.begin(), vUrls.end(), pLine) == vUrls.end())
				vUrls.emplace_back(pLine);
		}
	}
	if(vUrls.empty())
		for(const char *pUrl : DEFAULT_SERVERLIST_URLS)
			vUrls.emplace_back(pUrl);

	auto RequestFactory = [](const char *pUrl) {
		std::unique_ptr<IHttpRequest> pRequest = HttpGet(pUrl);
		return std::shared_ptr<IHttpRequest>(std::move(pRequest));
	};
	auto Clock = [] { return time_get() / time_freq(); };
	return CreateServerBrowserHttpWithSources(pEngine, pStorage, pHttp, std::move(vUrls), pPreviousBestUrl, RequestFactory, Clock);
}
