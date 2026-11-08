#ifndef ENGINE_STEAM_H
#define ENGINE_STEAM_H

#include "kernel.h"

#include <base/types.h>

class ISteam : public IInterface
{
	MACRO_INTERFACE("steam")
public:
	// Returns nullptr if the name cannot be determined.
	virtual const char *GetPlayerName() = 0;

	// Returns nullptr if the no server needs to be joined.
	// Can change while the game is running.
	virtual const NETADDR *GetConnectAddress() = 0;
	virtual void ClearConnectAddress() = 0;

	virtual void Update() = 0;

	virtual void ClearGameInfo() = 0;
	virtual void SetGameInfo(const NETADDR &ServerAddr, const char *pMapName, bool AnnounceAddr) = 0;

	// [STEAM-HIDE] 运行中隐藏：立即断开 Steam（重新启用需重启客户端）
	virtual void HideFromSteam() = 0;
};

// HideFromSteam=true 时不调用 SteamAPI_Init，返回 Stub（Steam 好友看不到"正在玩"）
ISteam *CreateSteam(bool HideFromSteam);

#endif // ENGINE_STEAM_H
