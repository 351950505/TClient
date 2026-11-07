#include "custom_communities.h"

#include <base/dbg.h>
#include <base/log.h>

#include <engine/client/serverbrowser.h>
#include <engine/external/json-parser/json.h>
#include <engine/shared/config.h>

static constexpr char CUSTOM_COMMUNITIES_DDNET_INFO_FILE[] = "custom-communities-ddnet-info.json";
static constexpr int64_t CUSTOM_COMMUNITIES_MAX_RESPONSE_SIZE = 4 * 1024 * 1024; // 现网约 19 KB，留足余量

void CCustomCommunities::ResetTask()
{
	if(m_pCustomCommunitiesDDNetInfoTask)
	{
		m_pCustomCommunitiesDDNetInfoTask->Abort();
		m_pCustomCommunitiesDDNetInfoTask = nullptr;
	}
}

void CCustomCommunities::DisableCustomCommunities()
{
	ResetTask();
	auto *pServerBrowser = dynamic_cast<CServerBrowser *>(ServerBrowser());
	dbg_assert(pServerBrowser, "pServerBrowser is nullptr");
	pServerBrowser->m_CustomCommunitiesFunction = nullptr;
	if(m_pCustomCommunitiesDDNetInfo)
		json_value_free(m_pCustomCommunitiesDDNetInfo);
	m_pCustomCommunitiesDDNetInfo = nullptr;
	m_CustomCommunitiesDDNetInfoSha256.reset();
	pServerBrowser->LoadDDNetServers();
}

// 解析并做结构校验：要求对象根；存在 "communities" 时必须是数组。
// 成功时接管 *ppJson 的所有权；失败时释放并返回 false。
bool CCustomCommunities::ParseCustomCommunitiesDDNetInfo(json_value *pJson, json_value **ppOut) const
{
	if(pJson == nullptr)
	{
		return false;
	}
	else if(pJson->type != json_object)
	{
		log_error("customcommunities", "invalid info root");
		json_value_free(pJson);
		return false;
	}

	const json_value &Communities = (*pJson)["communities"];
	if(Communities.type != json_array)
	{
		log_error("customcommunities", "invalid info: 'communities' must be an array");
		json_value_free(pJson);
		return false;
	}

	*ppOut = pJson;
	return true;
}

void CCustomCommunities::ApplyCustomCommunitiesDDNetInfo(json_value *pInfo, const std::optional<SHA256_DIGEST> &Digest)
{
	auto *pServerBrowser = dynamic_cast<CServerBrowser *>(ServerBrowser());
	dbg_assert(pServerBrowser, "pServerBrowser is nullptr");

	if(m_pCustomCommunitiesDDNetInfo)
		json_value_free(m_pCustomCommunitiesDDNetInfo);
	m_pCustomCommunitiesDDNetInfo = pInfo;
	m_CustomCommunitiesDDNetInfoSha256 = Digest;
	pServerBrowser->m_CustomCommunitiesFunction = [&](std::vector<json_value *> &vCommunities) {
		const json_value &Communities = (*m_pCustomCommunitiesDDNetInfo)["communities"];
		if(Communities.type == json_array)
		{
			vCommunities.insert(
				vCommunities.end(),
				Communities.u.array.values,
				Communities.u.array.values + Communities.u.array.length);
		}
	};
	pServerBrowser->LoadDDNetServers();
}

// 启动时从磁盘缓存先行加载（下载完成前就有社区列表可用）。
void CCustomCommunities::LoadCustomCommunitiesDDNetInfoFromDisk()
{
	void *pBuf;
	unsigned Length;
	if(!Storage()->ReadFile(CUSTOM_COMMUNITIES_DDNET_INFO_FILE, IStorage::TYPE_SAVE, &pBuf, &Length))
	{
		return;
	}
	json_settings JsonSettings{};
	char aError[256];
	json_value *pJson = json_parse_ex(&JsonSettings, static_cast<json_char *>(pBuf), Length, aError);
	const SHA256_DIGEST DiskDigest = sha256(pBuf, Length);
	free(pBuf);
	if(!ParseCustomCommunitiesDDNetInfo(pJson, &pJson))
	{
		log_error("customcommunities", "ignoring invalid cached info json: '%s'", aError);
		return;
	}
	ApplyCustomCommunitiesDDNetInfo(pJson, DiskDigest);
}

void CCustomCommunities::DownloadCustomCommunitiesDDNetInfo()
{
	if(g_Config.m_TcCustomCommunitiesUrl[0] == '\0')
	{
		DisableCustomCommunities();
		return;
	}

	ResetTask();
	// HttpGetBoth：写入磁盘的同时保留响应内存，便于先解析校验、再决定是否提交。
	std::shared_ptr<IHttpRequest> pTask = HttpGetBoth(g_Config.m_TcCustomCommunitiesUrl, Storage(), CUSTOM_COMMUNITIES_DDNET_INFO_FILE, IStorage::TYPE_SAVE);
	pTask->Timeout(CTimeout{10000, 0, 500, 10});
	pTask->MaxResponseSize(CUSTOM_COMMUNITIES_MAX_RESPONSE_SIZE);
	pTask->ValidateBeforeOverwrite(true); // 校验通过才落盘，损坏响应不覆盖 last-known-good
	pTask->SkipByFileTime(false); // Always re-download.
	// Use ipv4 so we can know the ingame ip addresses of players before they join game servers
	pTask->IpResolve(IPRESOLVE::V4);
	m_pCustomCommunitiesDDNetInfoTask = pTask;
	Http()->Run(pTask);
}

void CCustomCommunities::OnInit()
{
	DownloadCustomCommunitiesDDNetInfo();
}

void CCustomCommunities::OnConsoleInit()
{
	// Load Custom Communities from the file before the serverbrowser tries to use it
	LoadCustomCommunitiesDDNetInfoFromDisk();

	Console()->Chain(
		"tc_custom_communities_url", [](IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData) {
			pfnCallback(pResult, pCallbackUserData);
			if(pResult->NumArguments() > 0)
				((CCustomCommunities *)pUserData)->DownloadCustomCommunitiesDDNetInfo();
		},
		this);
}

void CCustomCommunities::OnRender()
{
	if(!m_pCustomCommunitiesDDNetInfoTask)
		return;

	const EHttpState State = m_pCustomCommunitiesDDNetInfoTask->State();
	if(State == EHttpState::RUNNING || State == EHttpState::QUEUED)
		return;

	// 结果 API（Result/ResultSha256/StatusCode）仅在 DONE 时合法；
	// ERROR/ABORTED 保留磁盘 last-known-good 与当前内存数据，本次结束。
	std::shared_ptr<IHttpRequest> pTask = std::move(m_pCustomCommunitiesDDNetInfoTask);
	m_pCustomCommunitiesDDNetInfoTask = nullptr;
	if(State != EHttpState::DONE)
	{
		log_error("customcommunities", "custom communities download failed (state %d); keeping last-known-good data", (int)State);
		return;
	}

	const SHA256_DIGEST DownloadDigest = pTask->ResultSha256();
	if(m_CustomCommunitiesDDNetInfoSha256.has_value() && *m_CustomCommunitiesDDNetInfoSha256 == DownloadDigest)
	{
		// 内容与已加载一致：提交落盘但跳过重新解析和列表重建。
		pTask->OnValidation(true);
		return;
	}

	unsigned char *pResult;
	size_t ResultSize;
	pTask->Result(&pResult, &ResultSize);

	json_settings JsonSettings{};
	char aError[256];
	json_value *pJson = json_parse_ex(&JsonSettings, reinterpret_cast<json_char *>(pResult), (int)ResultSize, aError);
	if(!ParseCustomCommunitiesDDNetInfo(pJson, &pJson))
	{
		// 拒绝损坏响应：删除临时文件，磁盘 last-known-good 保持不变。
		log_error("customcommunities", "rejecting invalid downloaded info json: '%s'", aError);
		pTask->OnValidation(false);
		return;
	}

	// 校验通过：提交落盘，再安装新数据。
	pTask->OnValidation(true);
	if(pTask->State() != EHttpState::DONE)
	{
		log_error("customcommunities", "download commit failed; keeping last-known-good data");
		json_value_free(pJson);
		return;
	}
	ApplyCustomCommunitiesDDNetInfo(pJson, DownloadDigest);
}

CCustomCommunities::~CCustomCommunities()
{
	m_pCustomCommunitiesDDNetInfoTask = nullptr;
	if(m_pCustomCommunitiesDDNetInfo)
		json_value_free(m_pCustomCommunitiesDDNetInfo);
}
