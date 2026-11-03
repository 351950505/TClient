// 控制台日志分类与筛选（纯函数，便于单测）。
// 移植自 QmClient 的 qm_console_log_filter.h（github.com/wxj881027/QmClient），
// 分类在写 backlog 时按结构化 system 名完成，不再解析整行文本。
#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_CONSOLE_LOG_FILTER_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_CONSOLE_LOG_FILTER_H

#include <base/str.h>

// 单行日志的归属类别（位标志，便于直接与筛选掩码做与运算）
enum
{
	TC_CONSOLE_LOG_CATEGORY_SYSTEM = 1 << 0, // 服务器与投票播报、网络、图形、存储、警告与报错
	TC_CONSOLE_LOG_CATEGORY_PLAYER = 1 << 1, // 玩家聊天、队伍聊天、私聊
	TC_CONSOLE_LOG_CATEGORY_COMMAND = 1 << 2, // 命令回复、命令回显、客户端提示
	TC_CONSOLE_LOG_CATEGORY_BINDS = 1 << 3, // bind / unbind / bindchat 回显
};

// 全部分类
#define TC_CONSOLE_LOG_CATEGORY_ALL (TC_CONSOLE_LOG_CATEGORY_SYSTEM | TC_CONSOLE_LOG_CATEGORY_PLAYER | TC_CONSOLE_LOG_CATEGORY_COMMAND | TC_CONSOLE_LOG_CATEGORY_BINDS)

/**
 * 判断日志的 system 名是否等于给定名字或是其子分类。
 *
 * 例如 "binds" 与 "binds/foo" 匹配 "binds"，而 "bindsomething" 不匹配，
 * 避免前缀匹配把无关 system 吞进同一个分类。
 */
inline bool TcConsoleLogSystemIs(const char *pSystem, const char *pName)
{
	if(!pSystem || !pName || pName[0] == '\0')
		return false;
	const char *pRest = str_startswith_nocase(pSystem, pName);
	return pRest != nullptr && (*pRest == '\0' || *pRest == '/');
}

/** 按日志的 system 名分类（`CLogMessage::m_aSystem`，写 backlog 时调用）。 */
inline int TcClassifyConsoleLogSystem(const char *pSystem)
{
	if(!pSystem || pSystem[0] == '\0')
		return TC_CONSOLE_LOG_CATEGORY_SYSTEM;

	if(TcConsoleLogSystemIs(pSystem, "binds") || TcConsoleLogSystemIs(pSystem, "bindchat"))
		return TC_CONSOLE_LOG_CATEGORY_BINDS;

	// 玩家说话：公共、队伍、私聊
	if(TcConsoleLogSystemIs(pSystem, "chat/all") ||
		TcConsoleLogSystemIs(pSystem, "chat/team") ||
		TcConsoleLogSystemIs(pSystem, "chat/whisper"))
		return TC_CONSOLE_LOG_CATEGORY_PLAYER;

	// 服务器播报、命令回复、命令回显、客户端提示
	if(TcConsoleLogSystemIs(pSystem, "chat/server") ||
		TcConsoleLogSystemIs(pSystem, "chatresp") ||
		TcConsoleLogSystemIs(pSystem, "console") ||
		TcConsoleLogSystemIs(pSystem, "chat/client"))
		return TC_CONSOLE_LOG_CATEGORY_COMMAND;

	return TC_CONSOLE_LOG_CATEGORY_SYSTEM;
}

/**
 * 把筛选掩码归一化到合法分类集合；空掩码退回全部分类。
 *
 * 空掩码会让控制台一行不剩，用户也没有可点的按钮能走出来，因此不保留这个状态。
 */
inline int TcNormalizeConsoleLogFilterMask(int Mask)
{
	const int Valid = Mask & TC_CONSOLE_LOG_CATEGORY_ALL;
	return Valid != 0 ? Valid : TC_CONSOLE_LOG_CATEGORY_ALL;
}

/** 「全部」只在所有分类开启时选中；点击它恢复全部，分类按钮独立切换。 */
inline bool TcConsoleLogFilterButtonActive(int Mask, int Category)
{
	return (TcNormalizeConsoleLogFilterMask(Mask) & Category) == Category;
}

inline int TcToggleConsoleLogFilterCategory(int Mask, int Category)
{
	if(Category == TC_CONSOLE_LOG_CATEGORY_ALL)
		return TC_CONSOLE_LOG_CATEGORY_ALL;
	return TcNormalizeConsoleLogFilterMask(TcNormalizeConsoleLogFilterMask(Mask) ^ (Category & TC_CONSOLE_LOG_CATEGORY_ALL));
}

/**
 * 单选式切换（TClient 交互）：点类别只看该类别，再点一次回到全部；点「全部」恢复全显。
 *
 * 与 QmClient 的多选风扇不同——玩家直觉是「点玩家=只看玩家」。
 */
inline int TcConsoleLogFilterSelectCategory(int Mask, int Category)
{
	if(Category == TC_CONSOLE_LOG_CATEGORY_ALL)
		return TC_CONSOLE_LOG_CATEGORY_ALL;
	const int Normalized = TcNormalizeConsoleLogFilterMask(Mask);
	const int CategoryBit = Category & TC_CONSOLE_LOG_CATEGORY_ALL;
	return Normalized == CategoryBit ? TC_CONSOLE_LOG_CATEGORY_ALL : CategoryBit;
}

/** 该行是否应该显示在给定筛选掩码下。 */
inline bool TcConsoleLogCategoryPassesFilter(int Category, int FilterMask)
{
	return (Category & TcNormalizeConsoleLogFilterMask(FilterMask)) != 0;
}

/** 顶栏按钮序号 → 类别（全部/玩家/系统/命令/绑定，与 QmClient 排版一致）。 */
inline int TcConsoleLogFilterButtonCategory(int ButtonIndex)
{
	switch(ButtonIndex)
	{
	case 0:
		return TC_CONSOLE_LOG_CATEGORY_ALL;
	case 1:
		return TC_CONSOLE_LOG_CATEGORY_PLAYER;
	case 2:
		return TC_CONSOLE_LOG_CATEGORY_SYSTEM;
	case 3:
		return TC_CONSOLE_LOG_CATEGORY_COMMAND;
	default:
		return TC_CONSOLE_LOG_CATEGORY_BINDS;
	}
}

#endif
