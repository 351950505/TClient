#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_CUSTOM_COMMUNITIES_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_CUSTOM_COMMUNITIES_H

#include <base/hash.h>

#include <engine/http.h>
#include <engine/shared/console.h>

#include <game/client/component.h>

#include <optional>

class CCustomCommunities : public CComponent
{
private:
	std::shared_ptr<IHttpRequest> m_pCustomCommunitiesDDNetInfoTask = nullptr;
	json_value *m_pCustomCommunitiesDDNetInfo = nullptr;
	// 内容变化 token：当前已加载的自定义信息字节的 SHA-256。它不是可信来源校验
	// （信任来自 HTTPS 与用户配置的 URL），只用于下载结果与已加载数据的去重比较。
	std::optional<SHA256_DIGEST> m_CustomCommunitiesDDNetInfoSha256;

	void DownloadCustomCommunitiesDDNetInfo();
	// 从磁盘缓存加载（仅启动时用于在下载完成前先行显示）。
	void LoadCustomCommunitiesDDNetInfoFromDisk();
	// 校验并接管解析结果；失败时释放 pJson 并返回 false。
	bool ParseCustomCommunitiesDDNetInfo(json_value *pJson, json_value **ppOut) const;
	// 安装已解析的信息并刷新服务器列表；digest 记录对应内容的变化 token。
	void ApplyCustomCommunitiesDDNetInfo(json_value *pInfo, const std::optional<SHA256_DIGEST> &Digest);
	// URL 为空时彻底停用：移除回调、释放数据并重置变化 token（否则重启用相同字节
	// 会走“未变化”快路径但回调已被移除）。
	void DisableCustomCommunities();
	void ResetTask();

public:
	void OnInit() override;
	void OnConsoleInit() override;
	void OnRender() override;
	int Sizeof() const override { return sizeof(*this); }
	~CCustomCommunities() override;
};

#endif
