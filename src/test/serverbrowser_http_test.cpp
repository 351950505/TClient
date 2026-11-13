#include "test.h"

#include <base/str.h>

#include <engine/client/serverbrowser_http.h>
#include <engine/engine.h>
#include <engine/http.h>
#include <engine/serverbrowser.h>
#include <engine/storage.h>

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{
	constexpr const char *SERVERLIST_STALE = R"({"servers":[{"addresses":["tw-0.6+udp://127.0.0.1:8303"],"info":{"max_clients":16,"max_players":16,"passworded":false,"game_type":"DDraceNetwork","name":"Stale","map":{"name":"Kobra"},"version":"0.7","clients":[]}}]})";
	constexpr const char *SERVERLIST_FRESH = R"({"servers":[{"addresses":["tw-0.6+udp://127.0.0.1:8304"],"info":{"max_clients":16,"max_players":16,"passworded":false,"game_type":"DDraceNetwork","name":"Fresh","map":{"name":"Kobra"},"version":"0.7","clients":[]}}]})";
	constexpr const char *SERVERLIST_FRESHEST = R"({"servers":[{"addresses":["tw-0.6+udp://127.0.0.1:8305"],"info":{"max_clients":16,"max_players":16,"passworded":false,"game_type":"DDraceNetwork","name":"Freshest","map":{"name":"Kobra"},"version":"0.7","clients":[]}}]})";

	class CServerListRequest : public IHttpRequest
	{
		std::string m_Url;

	public:
		explicit CServerListRequest(const char *pUrl) :
			IHttpRequest(pUrl),
			m_Url(pUrl)
		{
		}

		void Header(const char *) override {}

		void Start()
		{
			m_State = EHttpState::RUNNING;
		}

		void Complete(const char *pBody, std::optional<int64_t> Age)
		{
			EXPECT_EQ(State(), EHttpState::RUNNING);
			EXPECT_EQ(OnData(pBody, str_length(pBody)), (size_t)str_length(pBody));
			m_StatusCode = 200;
			if(Age.has_value())
			{
				m_ResultDate = 1000;
				m_ResultLastModified = 1000 - *Age;
			}
			OnCompletionInternal(EHttpState::DONE);
		}

		const char *Url() const { return m_Url.c_str(); }
	};

	class CHttpStub : public IHttp
	{
		std::vector<std::shared_ptr<CServerListRequest>> m_vRequests;

	public:
		void Run(std::shared_ptr<IHttpRequest> pRequest) override
		{
			auto pServerListRequest = std::static_pointer_cast<CServerListRequest>(std::move(pRequest));
			pServerListRequest->Start();
			m_vRequests.push_back(std::move(pServerListRequest));
		}

		bool HasIpresolveBug() const override { return false; }

		size_t NumRequests() const { return m_vRequests.size(); }
		const std::shared_ptr<CServerListRequest> &Request(size_t Index) const { return m_vRequests[Index]; }
	};

	std::shared_ptr<IHttpRequest> CreateRequest(const char *pUrl)
	{
		return std::make_shared<CServerListRequest>(pUrl);
	}
} // namespace

TEST(ServerBrowserHttp, StartsTwoAndPublishesFirstFreshResponse)
{
	CHttpStub Http;
	int64_t Now = 1000;
	std::unique_ptr<IServerBrowserHttp> pBrowser(CreateServerBrowserHttpWithSources(
		nullptr, nullptr, &Http,
		{"https://a.invalid/servers.json", "https://b.invalid/servers.json", "https://c.invalid/servers.json"},
		"https://c.invalid/servers.json", CreateRequest, [&Now] { return Now; }));

	ASSERT_EQ(Http.NumRequests(), 2u);
	EXPECT_STREQ(Http.Request(0)->Url(), "https://c.invalid/servers.json");
	EXPECT_STREQ(Http.Request(1)->Url(), "https://a.invalid/servers.json");
	EXPECT_TRUE(pBrowser->IsRefreshing());
	EXPECT_EQ(pBrowser->Revision(), 0u);

	Http.Request(1)->Complete(SERVERLIST_FRESH, 20);
	pBrowser->Update();

	ASSERT_EQ(pBrowser->NumServers(), 1);
	EXPECT_STREQ(pBrowser->Server(0).m_aName, "Fresh");
	EXPECT_EQ(pBrowser->Revision(), 1u);
	EXPECT_FALSE(pBrowser->IsRefreshing());
	EXPECT_TRUE(Http.Request(0)->IsAbortRequested());
	EXPECT_EQ(Http.NumRequests(), 2u);
	const char *pBestUrl;
	ASSERT_FALSE(pBrowser->GetBestUrl(&pBestUrl));
	EXPECT_STREQ(pBestUrl, "https://a.invalid/servers.json");
}

TEST(ServerBrowserHttp, ChoosesFreshestResponseWhenBothCompleteTogether)
{
	CHttpStub Http;
	int64_t Now = 1000;
	std::unique_ptr<IServerBrowserHttp> pBrowser(CreateServerBrowserHttpWithSources(
		nullptr, nullptr, &Http,
		{"https://a.invalid/servers.json", "https://b.invalid/servers.json"},
		nullptr, CreateRequest, [&Now] { return Now; }));

	ASSERT_EQ(Http.NumRequests(), 2u);
	Http.Request(0)->Complete(SERVERLIST_STALE, 299);
	Http.Request(1)->Complete(SERVERLIST_FRESHEST, 1);
	pBrowser->Update();

	ASSERT_EQ(pBrowser->NumServers(), 1);
	EXPECT_STREQ(pBrowser->Server(0).m_aName, "Freshest");
	EXPECT_EQ(pBrowser->Revision(), 1u);
	EXPECT_FALSE(pBrowser->IsRefreshing());
	const char *pBestUrl;
	ASSERT_FALSE(pBrowser->GetBestUrl(&pBestUrl));
	EXPECT_STREQ(pBestUrl, "https://b.invalid/servers.json");
}

TEST(ServerBrowserHttp, PublishesUnknownAgeThenReplacesItWithFreshResponse)
{
	CHttpStub Http;
	int64_t Now = 1000;
	std::unique_ptr<IServerBrowserHttp> pBrowser(CreateServerBrowserHttpWithSources(
		nullptr, nullptr, &Http,
		{"https://a.invalid/servers.json", "https://b.invalid/servers.json", "https://c.invalid/servers.json"},
		nullptr, CreateRequest, [&Now] { return Now; }));

	ASSERT_EQ(Http.NumRequests(), 2u);
	Http.Request(0)->Complete(SERVERLIST_STALE, std::nullopt);
	pBrowser->Update();

	ASSERT_EQ(pBrowser->NumServers(), 1);
	EXPECT_STREQ(pBrowser->Server(0).m_aName, "Stale");
	EXPECT_EQ(pBrowser->Revision(), 1u);
	EXPECT_EQ(Http.NumRequests(), 3u);
	EXPECT_TRUE(pBrowser->IsRefreshing());

	Http.Request(1)->Complete(SERVERLIST_FRESH, 10);
	pBrowser->Update();

	ASSERT_EQ(pBrowser->NumServers(), 1);
	EXPECT_STREQ(pBrowser->Server(0).m_aName, "Fresh");
	EXPECT_EQ(pBrowser->Revision(), 2u);
	EXPECT_TRUE(Http.Request(2)->IsAbortRequested());
}

TEST(ServerBrowserHttp, KeepsFresherPersistedListUntilBetterResponseArrives)
{
	CTestInfo Info;
	std::unique_ptr<IStorage> pStorage = Info.CreateTestStorage();
	ASSERT_NE(pStorage, nullptr);
	Info.m_DeleteTestStorageFilesOnSuccess = true;
	const std::vector<std::string> vUrls = {
		"https://a.invalid/servers.json",
		"https://b.invalid/servers.json"};
	int64_t Now = 1000;

	std::unique_ptr<IEngine> pSaveEngine(CreateTestEngine("serverbrowser_http_test"));
	{
		CHttpStub Http;
		std::unique_ptr<IServerBrowserHttp> pBrowser(CreateServerBrowserHttpWithSources(
			pSaveEngine.get(), pStorage.get(), &Http, vUrls, nullptr, CreateRequest, [&Now] { return Now; }));
		ASSERT_EQ(Http.NumRequests(), 2u);
		Http.Request(0)->Complete(SERVERLIST_FRESH, 15);
		pBrowser->Update();
		ASSERT_EQ(pBrowser->Revision(), 1u);
	}
	pSaveEngine->ShutdownJobs();
	ASSERT_TRUE(pStorage->FileExists("cache/tclient-serverlist.json", IStorage::TYPE_SAVE));

	std::unique_ptr<IEngine> pRefreshEngine(CreateTestEngine("serverbrowser_http_test"));
	CHttpStub Http;
	std::unique_ptr<IServerBrowserHttp> pBrowser(CreateServerBrowserHttpWithSources(
		pRefreshEngine.get(), pStorage.get(), &Http, vUrls, nullptr, CreateRequest, [&Now] { return Now; }));

	ASSERT_EQ(Http.NumRequests(), 2u);
	ASSERT_EQ(pBrowser->NumServers(), 1);
	EXPECT_STREQ(pBrowser->Server(0).m_aName, "Fresh");
	EXPECT_EQ(pBrowser->Revision(), 1u);
	EXPECT_TRUE(pBrowser->IsRefreshing());

	Http.Request(0)->Complete(SERVERLIST_STALE, 20);
	pBrowser->Update();
	EXPECT_STREQ(pBrowser->Server(0).m_aName, "Fresh");
	EXPECT_EQ(pBrowser->Revision(), 1u);
	EXPECT_TRUE(pBrowser->IsRefreshing());
	const char *pBestUrl;
	EXPECT_TRUE(pBrowser->GetBestUrl(&pBestUrl));

	Http.Request(1)->Complete(SERVERLIST_FRESHEST, 5);
	pBrowser->Update();
	EXPECT_STREQ(pBrowser->Server(0).m_aName, "Freshest");
	EXPECT_EQ(pBrowser->Revision(), 2u);
	EXPECT_FALSE(pBrowser->IsRefreshing());
	ASSERT_FALSE(pBrowser->GetBestUrl(&pBestUrl));
	EXPECT_STREQ(pBestUrl, "https://b.invalid/servers.json");

	pBrowser.reset();
	pRefreshEngine->ShutdownJobs();
}
