/* TClient: 观战选择器好友分列。学习 QmClient 的 spectator_friend_priority
   （https://github.com/wxj881027/QmClient ，src/game/client/components/qmclient/spectator_friend_priority.h），
   稳定分区算法与分列高度计算保持一致：
   - 好友保持原有相对顺序排在最前，其余玩家保持原有相对顺序紧随其后；
   - 不排序、不改组内相对顺序，pOrder 写出的是原数组下标；
   - 只做两次线性扫描，无分配，可在渲染路径上逐帧调用。 */
#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_SPECTATOR_FRIENDS_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_SPECTATOR_FRIENDS_H

#include <algorithm>

namespace tclient_spectator_friends
{
	// 返回好友数量，也就是好友分组在显示序列里的长度（好友分组必然是前缀）。
	// Enabled = false 时退化为原顺序，返回 0。
	inline int BuildFriendFirstOrder(const bool *pIsFriend, int Count, int *pOrder, bool Enabled = true)
	{
		if(Count <= 0)
			return 0;
		if(!Enabled)
		{
			for(int Index = 0; Index < Count; ++Index)
				pOrder[Index] = Index;
			return 0;
		}

		int Friends = 0;
		for(int Index = 0; Index < Count; ++Index)
		{
			if(pIsFriend[Index])
				pOrder[Friends++] = Index;
		}

		int Write = Friends;
		for(int Index = 0; Index < Count; ++Index)
		{
			if(!pIsFriend[Index])
				pOrder[Write++] = Index;
		}

		return Friends;
	}

	// 分列布局：好友占自己的列，其余玩家强制从新的一列开始，两段并排。
	// 返回内容总高度（含分组标题），按好友段与其余段分别取最高列。
	// 在 QmClient MaxColumnHeight 的基础上改为分段计算，实现真正的两列分栏。
	inline float SplitColumnHeight(int PlayerCount, int FriendCount, int PlayersPerColumn, float RowHeight, float TitleHeight)
	{
		if(PlayerCount <= 0 || PlayersPerColumn <= 0)
			return 0.0f;
		const int Friends = std::clamp(FriendCount, 0, PlayerCount);
		const int Others = PlayerCount - Friends;
		float Height = 0.0f;
		if(Friends > 0)
			Height = std::max(Height, TitleHeight + std::min(PlayersPerColumn, Friends) * RowHeight);
		if(Others > 0)
			Height = std::max(Height, (Friends > 0 ? TitleHeight : 0.0f) + std::min(PlayersPerColumn, Others) * RowHeight);
		return Height;
	}

	// 无分组标题、不分段时的最高列高（旧布局基准），用来推导面板需要额外长出多少。
	inline float BaseMaxColumnHeight(int PlayerCount, int PlayersPerColumn, float RowHeight)
	{
		if(PlayerCount <= 0 || PlayersPerColumn <= 0)
			return 0.0f;

		float Height = 0.0f;
		for(int First = 0; First < PlayerCount; First += PlayersPerColumn)
			Height = std::max(Height, std::min(PlayersPerColumn, PlayerCount - First) * (float)RowHeight);
		return Height;
	}
}

#endif
