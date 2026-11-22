/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_MENUS_CHECKBOX_HELP_H
#define GAME_CLIENT_COMPONENTS_MENUS_CHECKBOX_HELP_H

struct SCheckBoxHelpEntry
{
	const int *m_pVariable;
	const char *m_pHelp;
};

// 用通俗一句话说明勾选某个开关后会发生什么的悬停帮助表。
// 只在 TClient 和 ME 设置页由通用复选框查表显示；表外（普通 UI、
// 临时状态复选框）查不到就保持原样不显示提示。
// 文案存英文原文，渲染时经 TCLocalize 查词典。
extern const SCheckBoxHelpEntry g_aCheckBoxHelp[];

int NumCheckBoxHelpEntries();

// 按配置变量地址查通俗说明；查不到返回 nullptr。
const char *FindCheckBoxHelpText(const int *pVariable);

#endif
